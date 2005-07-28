import java.io.DataInput;
import java.io.IOException;
import java.util.HashMap;

public abstract class Module extends OpcodeFactory
{
	protected final short moduleNumber;
	protected final HashMap factories;
	
	protected Module(DataInput input, short moduleNumber)
	{
		super(input);
		this.moduleNumber = moduleNumber;
		factories = new HashMap();
	}
	
	public short getModuleNumber()
	{
		return moduleNumber;
	}
	
	void verifyModule() throws UnexpectedModuleException, IOException
	{
		short number = input.readShort();
		if(number != moduleNumber)
			throw new UnexpectedModuleException(number);
	}
	
	void addFactory(ModuleOpcodeFactory factory) throws BadInputException, IOException
	{
		short number = factory.getOpcodeNumber();
		//Short key = Short.valueOf(number);
		Short key = new Short(number);
		if(factories.containsKey(key))
			throw new RuntimeException("Duplicate factory registered!");
		factories.put(key, factory);
		
		verifyModule();
		factory.verifyOpcode();
		factory.verifyName();
		factory.verifyParameters();
	}
	
	public Opcode readOpcode() throws BadInputException, IOException
	{
		short number = input.readShort();
		//Short key = Short.valueOf(number);
		Short key = new Short(number);
		ModuleOpcodeFactory factory = (ModuleOpcodeFactory) factories.get(key);
		if(factory == null)
			throw new UnexpectedOpcodeException(number);
		System.out.print("[" + factory.getOpcodeName() + "] ");
		return factory.readOpcode();
	}
	
	public static String render(short moduleNumber)
	{
		return Opcode.render(moduleNumber);
	}
}
