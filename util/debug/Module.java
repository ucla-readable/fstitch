import java.io.IOException;
import java.util.HashMap;

public abstract class Module extends OpcodeFactory
{
	protected final short moduleNumber;
	protected final HashMap factories;
	
	protected Module(CountingDataInput input, short moduleNumber)
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
			throw new UnexpectedModuleException(number, input.getOffset());
	}
	
	void addFactory(ModuleOpcodeFactory factory) throws BadInputException, IOException
	{
		//Short key = Short.valueOf(factory.opcodeNumber);
		Short key = new Short(factory.opcodeNumber);
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
			throw new UnexpectedOpcodeException(number, input.getOffset());
		return factory.readOpcode();
	}
	
	public static String hex(short moduleNumber)
	{
		return Opcode.hex(moduleNumber);
	}
}
