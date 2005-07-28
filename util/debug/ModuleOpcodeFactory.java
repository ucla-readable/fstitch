import java.io.*;
import java.util.Vector;

public abstract class ModuleOpcodeFactory extends OpcodeFactory
{
	protected final short opcodeNumber;
	private final Vector parameterNames;
	private final Vector parameterSizes;
	private int parameterCount;
	
	protected ModuleOpcodeFactory(DataInput input, short opcodeNumber)
	{
		super(input);
		this.opcodeNumber = opcodeNumber;
		parameterNames = new Vector();
		parameterSizes = new Vector();
		parameterCount = 0;
	}
	
	public short getOpcodeNumber()
	{
		return opcodeNumber;
	}
	
	protected void addParameter(String name, int size)
	{
		parameterNames.add(name);
		//parameterSizes.add(Integer.valueOf(size));
		parameterSizes.add(new Integer(size));
		parameterCount++;
	}
	
	protected String readString() throws IOException
	{
		ByteArrayOutputStream string = new ByteArrayOutputStream();
		byte b = input.readByte();
		while(b != 0)
		{
			string.write(b);
			b = input.readByte();
		}
		return new BufferedReader(new InputStreamReader(new ByteArrayInputStream(string.toByteArray()))).readLine();
	}
	
	public void verifyOpcode() throws UnexpectedOpcodeException, IOException
	{
		short number = input.readShort();
		if(number != opcodeNumber)
			throw new UnexpectedOpcodeException(number);
	}
	
	public abstract void verifyName() throws UnexpectedNameException, IOException;
	
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
}
