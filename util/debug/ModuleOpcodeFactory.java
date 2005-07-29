import java.io.DataInput;
import java.io.IOException;
import java.util.Vector;
import java.lang.reflect.InvocationTargetException;

public class ModuleOpcodeFactory extends OpcodeFactory
{
	public final short opcodeNumber;
	public final String opcodeName;
	public final Class opcodeClass;
	
	private final Vector parameterNames;
	private final Vector parameterSizes;
	private int parameterCount;
	
	public ModuleOpcodeFactory(DataInput input, short opcodeNumber, String opcodeName, Class opcodeClass)
	{
		super(input);
		this.opcodeNumber = opcodeNumber;
		this.opcodeName = opcodeName;
		this.opcodeClass = opcodeClass;
		parameterNames = new Vector();
		parameterSizes = new Vector();
		parameterCount = 0;
	}
	
	/* restrict this to early use */
	public void addParameter(String name, int size)
	{
		parameterNames.add(name);
		//parameterSizes.add(Integer.valueOf(size));
		parameterSizes.add(new Integer(size));
		parameterCount++;
	}
	
	public void verifyOpcode() throws UnexpectedOpcodeException, IOException
	{
		short number = input.readShort();
		if(number != opcodeNumber)
			throw new UnexpectedOpcodeException(number);
	}
	
	public void verifyName() throws UnexpectedNameException, IOException
	{
		String name = readString();
		if(!name.equals(opcodeName))
			throw new UnexpectedNameException(name);
	}
	
	private boolean checkParameter(int index, String name, int size)
	{
		String expName = (String) parameterNames.get(index);
		int expSize = ((Integer) parameterSizes.get(index)).intValue();
		return expName.equals(name) && expSize == size;
	}
	
	public void verifyParameters() throws UnexpectedParameterException, MissingParameterException, IOException
	{
		int index = 0;
		String name = null;
		
		byte size = input.readByte();
		while(size != 0 && index < parameterCount)
		{
			name = readString();
			if(!checkParameter(index, name, size))
				throw new UnexpectedParameterException(name, size);
			index++;
			size = input.readByte();
		}
		
		if(size != 0)
			throw new UnexpectedParameterException(readString(), size);
		if(index < parameterCount)
			throw new MissingParameterException((String) parameterNames.get(index), ((Integer) parameterSizes.get(index)).intValue());
	}
	
	private Opcode readGenericOpcode() throws UnexpectedParameterException, MissingParameterException, IOException, NoSuchMethodException,
	                                          InstantiationException, IllegalAccessException, InvocationTargetException
	{
		int index = 0;
		Vector parameters = new Vector();
		Vector types = new Vector();
		
		byte size = input.readByte();
		while(size != 0 && index < parameterCount)
		{
			switch(size)
			{
				case 1:
					//parameters.add(Byte.valueOf(input.readByte()));
					parameters.add(new Byte(input.readByte()));
					types.add(Byte.TYPE);
					break;
				case 2:
					//parameters.add(Short.valueOf(input.readShort()));
					parameters.add(new Short(input.readShort()));
					types.add(Short.TYPE);
					break;
				case 4:
					//parameters.add(Integer.valueOf(input.readInt()));
					parameters.add(new Integer(input.readInt()));
					types.add(Integer.TYPE);
					break;
				default:
					throw new UnexpectedParameterException("[" + opcodeName + ":" + index + "]", size);
			}
			index++;
			size = input.readByte();
		}
		
		if(size != 0)
			throw new UnexpectedParameterException("[" + opcodeName + ":" + index + "]", size);
		
		size = input.readByte();
		if(size != 0)
			System.err.println("Got parameter error flag while reading [" + opcodeName + "]");
		
		if(index < parameterCount)
			throw new MissingParameterException((String) parameterNames.get(index), ((Integer) parameterSizes.get(index)).intValue());
		
		/* magic reflection code to call the constructor */
		return (Opcode) opcodeClass.getConstructor((Class[]) types.toArray(new Class[types.size()])).newInstance(parameters.toArray());
	}
	
	public Opcode readOpcode() throws BadInputException, IOException
	{
		try {
			return readGenericOpcode();
		}
		catch(NoSuchMethodException e)
		{
			throw new BadInputException("Internal opcode class parameter error", e);
		}
		catch(InstantiationException e)
		{
			throw new BadInputException("Internal opcode class attribute error", e);
		}
		catch(IllegalAccessException e)
		{
			throw new BadInputException("Internal opcode class protection error", e);
		}
		catch(InvocationTargetException e)
		{
			throw new BadInputException(e.getTargetException());
		}
	}
}
